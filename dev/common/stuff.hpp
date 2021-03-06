#pragma once

#include <so_5/all.hpp>

using namespace std;
using namespace chrono_literals;

using namespace so_5;

// Сообщение для проверки одного файла с email-ом.
struct check_request {
  // Имя проверяемого файла.
  string email_file_;
  // Кому нужно отослать результат проверки.
  so_5::mbox_t reply_to_;
};

// Статус проверки, который будет возвращен в ответном сообщении.
enum class check_status {
  safe,
  suspicious,
  dangerous,
  check_failure,
  check_timedout
};

ostream & operator<<( ostream & to, check_status st ) {
  const char * v = "safe";
  if( check_status::suspicious == st ) v = "suspicious";
  else if( check_status::dangerous == st ) v = "dangerous";
  else if( check_status::check_failure == st ) v = "check_failure";
  else if( check_status::check_timedout == st ) v = "check_timedout";

  return (to << v);
}

// Сообщение с результатом проверки одного файла с email.
// Содержит не только статус проверки, но и имя проверяемого файла.
// Это имя нужно лишь для того, чтобы облегчить сопоставление
// получаемых результатов проверки.
struct check_result {
  string email_file_;
  check_status status_;
};

//
// Средства для имитации основных действий агентов.
//

string load_email_from_file( const string & file_name ) {
  // Просто имитируем паузу в зависимости от длины имени файла.
  this_thread::sleep_for( chrono::milliseconds(
        file_name.length() * 10 ) );
  return string();
}

class parsed_email {
  vector< string > headers_;
  string body_;
  vector< string > attachments_;

public :
  const auto & headers() const { return headers_; }
  const auto & body() const { return body_; }
  const auto & attachments() const { return attachments_; }
};

shared_ptr< parsed_email > parse_email( const string & ) {
  return make_shared< parsed_email >();
}

check_status check_headers( const vector< string > & ) {
  return check_status::safe;
}

check_status check_body( const string & ) {
  return check_status::safe;
}

check_status check_attachments( const vector< string > & ) {
  return check_status::safe;
}

//
// Агент, который будет инициировать последовательность запросов
// на проверку email-ов и будет собирать результаты проверок.
//
class requests_initiator final : public agent_t {
  struct initiate_next : public signal_t {};

public :
  requests_initiator(
    context_t ctx,
    mbox_t checker_mbox,
    size_t total_requests )
    : agent_t( ctx )
    , checker_( move(checker_mbox) )
    , total_requests_( total_requests )
  {
    so_subscribe_self()
      .event< initiate_next >( &requests_initiator::on_next )
      .event( &requests_initiator::on_result );
  }

  virtual void so_evt_start() override {
    send< initiate_next >( *this );
  }

private :
  const mbox_t checker_;

  const size_t total_requests_;
  size_t requests_sent_{ 0 };
  size_t results_received_{ 0 };

  void on_next() {
    // Инициируем запрос на провеку.
    send< check_request >(
        checker_,
        "email_" + to_string( requests_sent_ ) + ".mbox",
        so_direct_mbox() );

    ++requests_sent_;
    if( requests_sent_ < total_requests_ )
      send< initiate_next >( *this );
  }

  void on_result( const check_result & msg ) {
    cout << msg.email_file_ << " -> " << msg.status_ << endl;

    ++results_received_;
    if( results_received_ >= total_requests_ )
      // Работу всего приложения можно завершать.
      so_environment().stop();
  }
};

template< typename manager_type >
void do_imitation( size_t total_requests ) {
  // Запускаем SObjectizer Environment и сразу же указываем,
  // какие действия должны быть выполнены при старте.
  // Завершение работы приложения будет выполнено когда имитатор
  // получит ответы на все свои запросы.
  so_5::launch( [=]( environment_t & env ) {
    // Сначала отдельной кооперацией запускаем агента-менеджера.
    mbox_t checker_mbox;
    env.introduce_coop( [&]( coop_t & coop ) {
      auto manager = coop.make_agent< manager_type >();
      // mbox агента-менеджера потребуется для формирования потока запросов.
      checker_mbox = manager->so_direct_mbox();
    } );

    // Следующей кооперацией будет кооперация с агентом-имитатором запросов.
    // Запускаем имитатор запроса на собственной рабочей нити, дабы обработка
    // его сообщений выполнялась независимо от обработки сообщений
    // агента-менеджера.
    env.introduce_coop(
      disp::one_thread::create_private_disp( env )->binder(),
      [checker_mbox, total_requests]( coop_t & coop ) {
        coop.make_agent< requests_initiator >( checker_mbox, total_requests );
      } );
  } );
}

